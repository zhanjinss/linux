/*
 * Copyright 2013 Texas Instruments, Inc.
 *      Russ Dill <russ.dill@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */

#ifndef _LINUX_PIE_H
#define _LINUX_PIE_H

#include <linux/kernel.h>
#include <linux/err.h>

#include <asm/fncpy.h>
#include <linux/bug.h>

struct gen_pool;
struct pie_chunk;

#ifdef CONFIG_PIE

/**
 * __pie_load_data - load and fixup PIE code from kernel data
 * @pool:	pool to allocate memory from and copy code into
 * @start:	virtual start address in kernel of chunk specific code
 * @end:	virtual end address in kernel of chunk specific code
 * @phys:	%true to fixup to physical address of destination, %false to
 *		fixup to virtual address of destination
 *
 * Returns 0 on success, -EERROR otherwise
 */
struct pie_chunk *__pie_load_data(struct gen_pool *pool,
				  void *start, void *end, bool phys);

/**
 * pie_to_phys - translate a virtual PIE address into a physical one
 * @chunk:	identifier returned by pie_load_sections
 * @addr:	virtual address within pie chunk
 *
 * Returns physical address on success, -1 otherwise
 */
phys_addr_t pie_to_phys(struct pie_chunk *chunk, unsigned long addr);

void __iomem *__kern_to_pie(struct pie_chunk *chunk, void *ptr);

/**
 * pie_free - free the pool space used by an pie chunk
 * @chunk:	identifier returned by pie_load_sections
 */
void pie_free(struct pie_chunk *chunk);

#define __pie_load_sections(pool, name, folder, phys) ({		\
	extern char _binary_##folder##_pie_bin_start[];			\
	extern char __pie_##name##_start[];				\
	extern char __pie_##name##_end[];				\
	char *start = _binary_##folder##_pie_bin_start + (unsigned long)__pie_##name##_start; \
	char *end = _binary_##folder##_pie_bin_start + (unsigned long)__pie_##name##_end; \
									\
	__pie_load_data(pool, start, end, phys);			\
})

/*
 * Required for any symbol within an PIE section that is referenced by the
 * kernel
 */
#define EXPORT_PIE_SYMBOL(sym)		extern typeof(sym) sym __weak

#else

static inline struct pie_chunk *__pie_load_data(struct gen_pool *pool,
						void *start, void *end,
						bool phys)
{
	return ERR_PTR(-EINVAL);
}

static inline phys_addr_t pie_to_phys(struct pie_chunk *chunk,
				      unsigned long addr)
{
	return -1;
}

static inline void __iomem *__kern_to_pie(struct pie_chunk *chunk, void *ptr)
{
	return NULL;
}

static inline void pie_free(struct pie_chunk *chunk)
{
}

#define __pie_load_sections(pool, name, folder, phys) ({ ERR_PTR(-EINVAL); })

#endif

/**
 * pie_load_sections - load and fixup sections associated with the given name
 * @pool:	pool to allocate memory from and copy code into
 *		fixup to virtual address of destination
 * @name:	the name given to __pie() and __pie_data() when marking
 *		data and code
 *
 * Returns 0 on success, -EERROR otherwise
 */
#define pie_load_sections(pool, name, folder) ({			\
	__pie_load_sections(pool, name, folder, false);			\
})

/**
 * pie_load_sections_phys - load and fixup sections associated with the given
 * name for execution with the MMU off
 *
 * @pool:	pool to allocate memory from and copy code into
 *		fixup to virtual address of destination
 * @name:	the name given to __pie() and __pie_data() when marking
 *		data and code
 *
 * Returns 0 on success, -EERROR otherwise
 */
#define pie_load_sections_phys(pool, name, folder) ({			\
	__pie_load_sections(pool, name, folder, true);			\
})

/**
 * kern_to_pie - convert a kernel symbol to the virtual address of where
 * that symbol is loaded into the given PIE chunk.
 *
 * @chunk:	identifier returned by pie_load_sections
 * @p:		symbol to convert
 *
 * Return type is the same as type passed
 */
#define kern_to_pie(chunk, p) ({					\
	void *__ptr = (void *)(p);					\
	typeof(p) __result = (typeof(p))__kern_to_pie(chunk, __ptr);	\
	__result;							\
})

/**
 * kern_to_fn - convert a kernel function symbol to the virtual address of where
 * that symbol is loaded into the given PIE chunk
 *
 * @chunk:	identifier returned by pie_load_sections
 * @funcp:	function to convert
 *
 * Return type is the same as type passed
 */
#define fn_to_pie(chunk, funcp) ({					\
	uintptr_t __kern_addr, __pie_addr;				\
									\
	__kern_addr = (uintptr_t)funcp;					\
	__pie_addr = kern_to_pie(chunk, __kern_addr);			\
									\
	(typeof(&funcp))(__pie_addr);					\
})

#endif
