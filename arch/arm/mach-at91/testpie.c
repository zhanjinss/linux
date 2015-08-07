#include <linux/module.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/genalloc.h>

static void __iomem *addr;
static struct gen_pool *sram_pool;
static unsigned long sram_base;

static void (*plopfunc_ptr)(void);
static void plopfunc(void)
{
	int i;
	char str[] = "\r\nplop\r\n\r\n";

	for (i = 0; i < strlen(str); i++) {
		while (!(readl(addr + 0x14) & (1 << 1)))
		{
		}
		writel(str[i], addr + 0x1c);
	}
}

int __init test_init(void)
{
	phys_addr_t sram_pbase;
	struct device_node *node;
	struct platform_device *pdev = NULL;

	addr = ioremap(0xffffee00, SZ_4K);

	for_each_compatible_node(node, NULL, "mmio-sram") {
		pdev = of_find_device_by_node(node);
		if (pdev) {
			of_node_put(node);
			break;
		}
	}

	if (!pdev) {
		pr_warn("%s: failed to find sram device!\n", __func__);
		return -EINVAL;
	}

	sram_pool = dev_get_gen_pool(&pdev->dev);
	if (!sram_pool) {
		pr_warn("%s: sram pool unavailable!\n", __func__);
		return -EINVAL;
	}

	sram_base = gen_pool_alloc(sram_pool, 1024);
	if (!sram_base) {
		pr_warn("%s: unable to alloc sram!\n", __func__);
		return -EINVAL;
	}

	sram_pbase = gen_pool_virt_to_phys(sram_pool, sram_base);
	plopfunc_ptr = __arm_ioremap_exec(sram_pbase,
					1024, false);
	if (!plopfunc_ptr) {
		pr_warn("SRAM: Could not map\n");
	}

	plopfunc();

	return 0;
}

void __exit test_exit(void)
{
	gen_pool_free(sram_pool, sram_base, 1024);

	iounmap(addr);
}

module_init(test_init);
module_exit(test_exit);

MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Your description");
MODULE_LICENSE("GPL");

