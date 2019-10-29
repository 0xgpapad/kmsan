=============================
KernelMemorySanitizer (KMSAN)
=============================

KMSAN is a dynamic memory error detector aimed at finding uses of uninitialized
memory.
It is based on compiler instrumentation, and is quite similar to the userspace
MemorySanitizer tool (http://clang.llvm.org/docs/MemorySanitizer.html).

KMSAN and Clang
===============

In order for KMSAN to work the kernel must be
built with Clang, which is so far the only compiler that has KMSAN support.
The kernel instrumentation pass is based on the userspace MemorySanitizer tool
(http://clang.llvm.org/docs/MemorySanitizer.html). Because of the
instrumentation complexity it's unlikely that any other compiler will support
KMSAN soon.

Right now the instrumentation pass supports x86_64 only.

How to build
============

In order to build a kernel with KMSAN you'll need a fresh Clang (10.0.0+, trunk
version r365008 or greater). Please refer to
https://llvm.org/docs/GettingStarted.html for the instructions on how to build
Clang::

  export KMSAN_CLANG_PATH=/path/to/clang
  # Now configure and build the kernel with CONFIG_KMSAN enabled.
  make CC=$KMSAN_CLANG_PATH -j64

How KMSAN works
===============

KMSAN shadow memory
-------------------

KMSAN associates a so-called shadow byte with every byte of kernel memory.
A bit in the shadow byte is set iff the corresponding bit of the kernel memory
byte is uninitialized.
Marking the memory uninitialized (i.e. setting its shadow bytes to 0xff) is
called poisoning, marking it initialized (setting the shadow bytes to 0x00) is
called unpoisoning.

When a new variable is allocated on the stack, it's poisoned by default by
instrumentation code inserted by the compiler (unless it's a stack variable that
is immediately initialized). Any new heap allocation done without ``__GFP_ZERO``
is also poisoned.

Compiler instrumentation also tracks the shadow values with the help from the
runtime library in ``mm/kmsan/``.

The shadow value of a basic or compound type is an array of bytes of the same
length.
When a constant value is written into memory, that memory is unpoisoned.
When a value is read from memory, its shadow memory is also obtained and
propagated into all the operations which use that value. For every instruction
that takes one or more values the compiler generates code that calculates the
shadow of the result depending on those values and their shadows.

Example::

  int a = 0xff;
  int b;
  int c = a | b;

In this case the shadow of ``a`` is ``0``, shadow of ``b`` is ``0xffffffff``,
shadow of ``c`` is ``0xffffff00``. This means that the upper three bytes of
``c`` are uninitialized, while the lower byte is initialized.


Origin tracking
---------------

Every four bytes of kernel memory also have a so-called origin assigned to
them.
This origin describes the point in program execution at which the uninitialized
value was created. Every origin is associated with a creation stack, which lets
the user figure out what's going on.

When an uninitialized variable is allocated on stack or heap, a new origin
value is created, and that variable's origin is filled with that value.
When a value is read from memory, its origin is also read and kept together
with the shadow. For every instruction that takes one or more values the origin
of the result is one of the origins corresponding to any of the uninitialized
inputs.
If a poisoned value is written into memory, its origin is written to the
corresponding storage as well.

Example 1::

  int a = 0;
  int b;
  int c = a + b;

In this case the origin of ``b`` is generated upon function entry, and is
stored to the origin of ``c`` right before the addition result is written into
memory.

Several variables may share the same origin address, if they are stored in the
same four-byte chunk.
In this case every write to either variable updates the origin for all of them.

Example 2::

  int combine(short a, short b) {
    union ret_t {
      int i;
      short s[2];
    } ret;
    ret.s[0] = a;
    ret.s[1] = b;
    return ret.i;
  }

If ``a`` is initialized and ``b`` is not, the shadow of the result would be
0xffff0000, and the origin of the result would be the origin of ``b``.
``ret.s[0]`` would have the same origin, but it will be never used, because
that variable is initialized.

If both function arguments are uninitialized, only the origin of the second
argument is preserved.

Origin chaining
~~~~~~~~~~~~~~~
To ease the debugging, KMSAN creates a new origin for every memory store.
The new origin references both its creation stack and the previous origin the
memory location had.
This may cause increased memory consumption, so we limit the length of origin
chains in the runtime.

Clang instrumentation API
-------------------------

Clang instrumentation pass inserts calls to functions defined in
``mm/kmsan/kmsan_instr.c`` into the kernel code.

Shadow manipulation
~~~~~~~~~~~~~~~~~~~
For every memory access the compiler emits a call to a function that returns a
pair of pointers to the shadow and origin addresses of the given memory::

  typedef struct {
    void *s, *o;
  } shadow_origin_ptr_t

  shadow_origin_ptr_t __msan_metadata_ptr_for_load_{1,2,4,8}(void *addr)
  shadow_origin_ptr_t __msan_metadata_ptr_for_store_{1,2,4,8}(void *addr)
  shadow_origin_ptr_t __msan_metadata_ptr_for_load_n(void *addr, u64 size)
  shadow_origin_ptr_t __msan_metadata_ptr_for_store_n(void *addr, u64 size)

The function name depends on the memory access size.
Each such function also checks if the shadow of the memory in the range
[``addr``, ``addr + n``) is contiguous and reports an error otherwise.

The compiler makes sure that for every loaded value its shadow and origin
values are read from memory.
When a value is stored to memory, its shadow and origin are also stored using
the metadata pointers.

Origin tracking
~~~~~~~~~~~~~~~
A special function is used to create a new origin value for a local variable
and set the origin of that variable to that value::

  void __msan_poison_alloca(u64 address, u64 size, char *descr)

Access to per-task data
~~~~~~~~~~~~~~~~~~~~~~~~~

At the beginning of every instrumented function KMSAN inserts a call to
``__msan_get_context_state()``::

  kmsan_context_state *__msan_get_context_state(void)

``kmsan_context_state`` is declared in ``include/linux/kmsan.h``::

  struct kmsan_context_s {
    char param_tls[KMSAN_PARAM_SIZE];
    char retval_tls[RETVAL_SIZE];
    char va_arg_tls[KMSAN_PARAM_SIZE];
    char va_arg_origin_tls[KMSAN_PARAM_SIZE];
    u64 va_arg_overflow_size_tls;
    depot_stack_handle_t param_origin_tls[PARAM_ARRAY_SIZE];
    depot_stack_handle_t retval_origin_tls;
    depot_stack_handle_t origin_tls;
  };

This structure is used by KMSAN to pass parameter shadows and origins between
instrumented functions.

String functions
~~~~~~~~~~~~~~~~

The compiler replaces calls to ``memcpy()``/``memmove()``/``memset()`` with the
following functions. These functions are also called when data structures are
initialized or copied, making sure shadow and origin values are copied alongside
with the data::

  void *__msan_memcpy(void *dst, void *src, u64 n)
  void *__msan_memmove(void *dst, void *src, u64 n)
  void *__msan_memset(void *dst, int c, size_t n)

Error reporting
~~~~~~~~~~~~~~~

For each pointer dereference and each condition the compiler emits a shadow
check that calls ``__msan_warning()`` in the case a poisoned value is being
used::

  void __msan_warning(u32 origin)

``__msan_warning()`` causes KMSAN runtime to print an error report.

Inline assembly instrumentation
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

KMSAN instruments every inline assembly output with a call to::

  void __msan_instrument_asm_store(u64 addr, u64 size)

, which unpoisons the memory region.

This approach may mask certain errors, but it also helps to avoid a lot of
false positives in bitwise operations, atomics etc.

Sometimes the pointers passed into inline assembly don't point to valid memory.
In such cases they are ignored at runtime.

Disabling the instrumentation
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
A function can be marked with ``__no_sanitize_memory``.
Doing so doesn't remove KMSAN instrumentation from it, however it makes the
compiler ignore the uninitialized values coming from the function's inputs,
and initialize the function's outputs.
The compiler won't inline functions marked with this attribute into functions
not marked with it, and vice versa.

It's also possible to disable KMSAN for a single file (e.g. main.o)::

  KMSAN_SANITIZE_main.o := n

or for the whole directory::

  KMSAN_SANITIZE := n

in the Makefile. This comes at a cost however: stack allocations from such files
and parameters of instrumented functions called from them will have incorrect
shadow/origin values. As a rule of thumb, avoid using KMSAN_SANITIZE.

Runtime library
---------------
The code is located in ``mm/kmsan/``.

Per-task KMSAN state
~~~~~~~~~~~~~~~~~~~~

Every task_struct has an associated KMSAN task state that holds the KMSAN
context (see above) and a per-task flag disallowing KMSAN reports::

  struct kmsan_task_state {
    ...
    bool allow_reporting;
    struct kmsan_context_state cstate;
    ...
  }

  struct task_struct {
    ...
    struct kmsan_task_state kmsan;
    ...
  }


KMSAN contexts
~~~~~~~~~~~~~~

When running in a kernel task context, KMSAN uses ``current->kmsan.cstate`` to
hold the metadata for function parameters and return values.

But in the case the kernel is running in the interrupt, softirq or NMI context,
where ``current`` is unavailable, KMSAN switches to per-cpu interrupt state::

  DEFINE_PER_CPU(kmsan_context_state[KMSAN_NESTED_CONTEXT_MAX],
                 kmsan_percpu_cstate);

Metadata allocation
~~~~~~~~~~~~~~~~~~~
There are several places in the kernel for which the metadata is stored.

1. Each ``struct page`` instance contains two pointers to its shadow and
origin pages::

  struct page {
    ...
    struct page *shadow, *origin;
    ...
  };

Every time a ``struct page`` is allocated, the runtime library allocates two
additional pages to hold its shadow and origins. This is done by adding hooks
to ``alloc_pages()``/``free_pages()`` in ``mm/page_alloc.c``.
To avoid allocating the metadata for non-interesting pages (right now only the
shadow/origin page themselves and stackdepot storage) the
``__GFP_NO_KMSAN_SHADOW`` flag is used.

There is a problem related to this allocation algorithm: when two contiguous
memory blocks are allocated with two different ``alloc_pages()`` calls, their
shadow pages may not be contiguous. So, if a memory access crosses the boundary
of a memory block, accesses to shadow/origin memory may potentially corrupt
other pages or read incorrect values from them.

As a workaround, we check the access size in
``__msan_metadata_ptr_for_XXX_YYY()`` and return a pointer to a fake shadow
region in the case of an error::

  char dummy_load_page[PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));
  char dummy_store_page[PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));

``dummy_load_page`` is zero-initialized, so reads from it always yield zeroes.
All stores to ``dummy_store_page`` are ignored.

Unfortunately at boot time we need to allocate shadow and origin pages for the
kernel data (``.data``, ``.bss`` etc.) and percpu memory regions, the size of
which is not a power of 2. As a result, we have to allocate the metadata page by
page, so that it is also non-contiguous, although it may be perfectly valid to
access the corresponding kernel memory across page boundaries.
This can be probably fixed by allocating 1<<N pages at once, splitting them and
deallocating the rest.

LSB of the ``shadow`` pointer in a ``struct page`` may be set to 1. In this case
shadow and origin pages are allocated, but KMSAN ignores accesses to them by
falling back to dummy pages. Allocating the metadata pages is still needed to
support ``vmap()/vunmap()`` operations on this struct page.

2. For vmalloc memory and modules, there's a direct mapping between the memory
range, its shadow and origin. KMSAN lessens the vmalloc area by 3/4, making only
the first quarter available to ``vmalloc()``. The second quarter of the vmalloc
area contains shadow memory for the first quarter, the third one holds the
origins. A small part of the fourth quarter contains shadow and origins for the
kernel modules. Please refer to ``arch/x86/include/asm/pgtable_64_types.h`` for
more details.

When an array of pages is mapped into a contiguous virtual memory space, their
shadow and origin pages are similarly mapped into contiguous regions.

3. For CPU entry area there're separate per-CPU arrays that hold its metadata::

  DEFINE_PER_CPU(char[CPU_ENTRY_AREA_SIZE], cpu_entry_area_shadow);
  DEFINE_PER_CPU(char[CPU_ENTRY_AREA_SIZE], cpu_entry_area_origin);

When calculating shadow and origin addresses for a given memory address, the
runtime checks whether the address belongs to the physical page range, the
virtual page range or CPU entry area.

Handling ``pt_regs``
~~~~~~~~~~~~~~~~~~~

Many functions receive a ``struct pt_regs`` holding the register state at a
certain point. Registers don't have (easily calculatable) shadow or origin
associated with them.
We can assume that the registers are always initialized.

Example report
--------------
Here's an example of a real KMSAN report in ``packet_bind_spkt()``::

  ==================================================================
  BUG: KMSAN: uninit-value in strlen
  CPU: 0 PID: 1074 Comm: packet Not tainted 4.8.0-rc6+ #1891
  Hardware name: QEMU Standard PC (i440FX + PIIX, 1996), BIOS Bochs 01/01/2011
   0000000000000000 ffff88006b6dfc08 ffffffff82559ae8 ffff88006b6dfb48
   ffffffff818a7c91 ffffffff85b9c870 0000000000000092 ffffffff85b9c550
   0000000000000000 0000000000000092 00000000ec400911 0000000000000002
  Call Trace:
   [<     inline     >] __dump_stack lib/dump_stack.c:15
   [<ffffffff82559ae8>] dump_stack+0x238/0x290 lib/dump_stack.c:51
   [<ffffffff818a6626>] kmsan_report+0x276/0x2e0 mm/kmsan/kmsan.c:1003
   [<ffffffff818a783b>] __msan_warning+0x5b/0xb0 mm/kmsan/kmsan_instr.c:424
   [<     inline     >] strlen lib/string.c:484
   [<ffffffff8259b58d>] strlcpy+0x9d/0x200 lib/string.c:144
   [<ffffffff84b2eca4>] packet_bind_spkt+0x144/0x230 net/packet/af_packet.c:3132
   [<ffffffff84242e4d>] SYSC_bind+0x40d/0x5f0 net/socket.c:1370
   [<ffffffff84242a22>] SyS_bind+0x82/0xa0 net/socket.c:1356
   [<ffffffff8515991b>] entry_SYSCALL_64_fastpath+0x13/0x8f arch/x86/entry/entry_64.o:?
  chained origin:
   [<ffffffff810bb787>] save_stack_trace+0x27/0x50 arch/x86/kernel/stacktrace.c:67
   [<     inline     >] kmsan_save_stack_with_flags mm/kmsan/kmsan.c:322
   [<     inline     >] kmsan_save_stack mm/kmsan/kmsan.c:334
   [<ffffffff818a59f8>] kmsan_internal_chain_origin+0x118/0x1e0 mm/kmsan/kmsan.c:527
   [<ffffffff818a7773>] __msan_set_alloca_origin4+0xc3/0x130 mm/kmsan/kmsan_instr.c:380
   [<ffffffff84242b69>] SYSC_bind+0x129/0x5f0 net/socket.c:1356
   [<ffffffff84242a22>] SyS_bind+0x82/0xa0 net/socket.c:1356
   [<ffffffff8515991b>] entry_SYSCALL_64_fastpath+0x13/0x8f arch/x86/entry/entry_64.o:?
  origin description: ----address@SYSC_bind (origin=00000000eb400911)
  ==================================================================

The report tells that the local variable ``address`` was created uninitialized
in ``SYSC_bind()`` (the ``bind`` system call implementation). The lower stack
trace corresponds to the place where this variable was created.

The upper stack shows where the uninit value was used - in ``strlen()``.
It turned out that the contents of ``address`` were partially copied from the
userspace, but the buffer wasn't zero-terminated and contained some trailing
uninitialized bytes.
``packet_bind_spkt()`` didn't check the length of the buffer, but called
``strlcpy()`` on it, which called ``strlen()``, which started reading the
buffer byte by byte till it hit the uninitialized memory.


References
==========

E. Stepanov, K. Serebryany. MemorySanitizer: fast detector of uninitialized
memory use in C++.
In Proceedings of CGO 2015.
