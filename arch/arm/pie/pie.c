/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/genalloc.h>
#include <linux/pie.h>
#include <asm/cacheflush.h>

struct pie_chunk {
	struct gen_pool *pool;
	unsigned long addr;
	unsigned int align_offset;
	size_t sz;
};

struct pie_chunk *__pie_load_data(struct gen_pool *pool, void *code_start,
				  void *code_end, unsigned int align)
{
	struct pie_chunk *chunk;
	unsigned long offset;
	int ret;
	size_t code_sz;
	unsigned long base;
	phys_addr_t pbase;

	chunk = kzalloc(sizeof(*chunk), GFP_KERNEL);
	if (!chunk) {
		ret = -ENOMEM;
		goto err;
	}

	code_sz = code_end - code_start;
	chunk->pool = pool;
	chunk->sz = code_sz;

	base = gen_pool_alloc(pool, chunk->sz + align);
	if (!base) {
		ret = -ENOMEM;
		goto err_free;
	}

	pbase = gen_pool_virt_to_phys(pool, base);
	chunk->addr = (unsigned long)__arm_ioremap_exec(pbase, code_sz, false);
	if (!chunk->addr) {
		ret = -ENOMEM;
		goto err_remap;
	}

	chunk->align_offset = chunk->addr % align;
	chunk->addr += chunk->align_offset;

	memcpy((char *)chunk->addr, code_start, code_sz);
	flush_icache_range(chunk->addr, chunk->addr + code_sz);

	offset = gen_pool_virt_to_phys(pool, chunk->addr);

	return chunk;

err_remap:
	gen_pool_free(chunk->pool, chunk->addr, chunk->sz);

err_free:
	kfree(chunk);
err:
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(__pie_load_data);

phys_addr_t pie_to_phys(struct pie_chunk *chunk, unsigned long addr)
{
	return gen_pool_virt_to_phys(chunk->pool, addr);
}
EXPORT_SYMBOL_GPL(pie_to_phys);

void __iomem *__kern_to_pie(struct pie_chunk *chunk, void *ptr)
{
	uintptr_t offset = (uintptr_t)ptr;

	if (offset >= chunk->sz)
		return NULL;
	else
		return (void *)(chunk->addr + offset);
}
EXPORT_SYMBOL_GPL(__kern_to_pie);

void pie_free(struct pie_chunk *chunk)
{
	gen_pool_free(chunk->pool, chunk->addr, chunk->sz);
	kfree(chunk);
}
EXPORT_SYMBOL_GPL(pie_free);
