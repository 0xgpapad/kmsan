// SPDX-License-Identifier: GPL-2.0
#include <linux/kmsan.h>
#include <linux/kmsan-checks.h>
#include <linux/uaccess.h>

/* out-of-line parts */

#ifndef INLINE_COPY_FROM_USER
unsigned long _copy_from_user(void *to, const void __user *from, unsigned long n)
{
	unsigned long res = n, to_copy = n;
	might_fault();
	if (likely(access_ok(VERIFY_READ, from, n))) {
		kasan_check_write(to, n);
		res = raw_copy_from_user(to, from, n);
		kmsan_unpoison_shadow(to, to_copy - res);
	}
	if (unlikely(res))
		memset(to + (n - res), 0, res);
	return res;
}
EXPORT_SYMBOL(_copy_from_user);
#endif

#ifndef INLINE_COPY_TO_USER
unsigned long _copy_to_user(void __user *to, const void *from, unsigned long n)
{
	unsigned long to_copy = n;
	might_fault();
	if (likely(access_ok(VERIFY_WRITE, to, n))) {
		kasan_check_read(from, n);
		n = raw_copy_to_user(to, from, n);
		kmsan_copy_to_user(to, from, to_copy, n);
	}
	return n;
}
EXPORT_SYMBOL(_copy_to_user);
#endif
