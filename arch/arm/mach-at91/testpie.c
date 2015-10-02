#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/genalloc.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/slab.h>
#include <linux/moduleloader.h>

static struct gen_pool *sram_pool;
static unsigned long sram_base;
static unsigned long sram_size;
void __iomem *addr;

static void (*plopfunc_ptr)(void __iomem *addr);

void *sram_alloc(unsigned long size)
{
	phys_addr_t sram_pbase;
	struct device_node *node;
	struct platform_device *pdev = NULL;

	for_each_compatible_node(node, NULL, "mmio-sram") {
		pdev = of_find_device_by_node(node);
		if (pdev) {
			of_node_put(node);
			break;
		}
	}

	if (!pdev) {
		pr_warn("%s: failed to find sram device!\n", __func__);
		return NULL;
	}

	sram_pool = dev_get_gen_pool(&pdev->dev);
	if (!sram_pool) {
		pr_warn("%s: sram pool unavailable!\n", __func__);
		return NULL;
	}

	sram_base = gen_pool_alloc(sram_pool, size);
	if (!sram_base) {
		pr_warn("%s: unable to alloc sram!\n", __func__);
		return NULL;
	}

	sram_pbase = gen_pool_virt_to_phys(sram_pool, sram_base);
	sram_size = size;

	return __arm_ioremap_exec(sram_pbase, size, false);
}

void sram_free(void* addr)
{
	gen_pool_free(sram_pool, sram_base, sram_size);
}

int __init test_init(void)
{
	const struct kernel_symbol *sym;

	pr_err("%s +%d %s\n", __FILE__, __LINE__, __func__);
	init_module_at("atmel_pm.ko", sram_alloc, sram_free);

	addr = ioremap(0xffffee00, SZ_512);
//	addr = ioremap(0xfc00c000, SZ_512);

	pr_err("%s +%d %s %p\n", __FILE__, __LINE__, __func__, addr);

	/* first, check if it's an exported symbol */
	preempt_disable();
	sym = find_symbol("plopfunc", NULL, NULL, true, true);
	if (sym) {
		plopfunc_ptr = sym->value;
	}
	preempt_enable();

	plopfunc_ptr(addr);

	pr_err("%s +%d %s %p\n", __FILE__, __LINE__, __func__, plopfunc_ptr);
	return 0;
}

void __exit test_exit(void)
{
	iounmap(addr);
}

module_init(test_init);
module_exit(test_exit);

MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Your description");
MODULE_LICENSE("GPL");

