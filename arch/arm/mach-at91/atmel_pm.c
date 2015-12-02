#include <linux/module.h>
#include <linux/genalloc.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/clk/at91_pmc.h>
#include <linux/mfd/syscon/atmel-mc.h>
#include "pm.h"


static struct gen_pool *sram_pool;
static unsigned long sram_base;
static unsigned long sram_size;

void __aeabi_unwind_cpp_pr0(void)
{
};

void __aeabi_unwind_cpp_pr1(void)
{
};

void __aeabi_unwind_cpp_pr2(void)
{
};

static void at91_sramc_self_refresh(unsigned int is_active,
				    unsigned int memtype,
				    void __iomem *sdramc_base,
				    void __iomem *sdramc_base1)
{
	static unsigned int lpr, mdr, lpr1, mdr1;

	switch(memtype)
	{
	case AT91_MEMCTRL_MC:
	/*
	 * at91rm9200 Memory controller
	 */
		if (is_active)
			__raw_writel(1, sdramc_base + AT91_MC_SDRAMC_SRR);
		break;

	case AT91_MEMCTRL_DDRSDR:
		if (is_active) {
			mdr = __raw_readl(sdramc_base + AT91_DDRSDRC_MDR);
			lpr = __raw_readl(sdramc_base + AT91_DDRSDRC_LPR);

			if ((mdr & AT91_DDRSDRC_MD) == AT91_DDRSDRC_MD_LOW_POWER_DDR)
				__raw_writel((mdr & ~AT91_DDRSDRC_MD) |
					     AT91_DDRSDRC_MD_DDR2, sdramc_base +
					     AT91_DDRSDRC_MDR);
			__raw_writel((lpr & ~AT91_DDRSDRC_LPCB) |
				     AT91_DDRSDRC_LPCB_SELF_REFRESH, sdramc_base
				     + AT91_DDRSDRC_LPR);

			if (sdramc_base1) {
				mdr1 = __raw_readl(sdramc_base1 + AT91_DDRSDRC_MDR);
				lpr1 = __raw_readl(sdramc_base1 + AT91_DDRSDRC_LPR);
				if ((mdr1 & AT91_DDRSDRC_MD) == AT91_DDRSDRC_MD_LOW_POWER_DDR)
					__raw_writel((mdr1 & ~AT91_DDRSDRC_MD) |
						     AT91_DDRSDRC_MD_DDR2,
						     sdramc_base1 +
						     AT91_DDRSDRC_MDR);
				__raw_writel((lpr1 & ~AT91_DDRSDRC_LPCB) |
					     AT91_DDRSDRC_LPCB_SELF_REFRESH,
					     sdramc_base1 + AT91_DDRSDRC_LPR);
			}
		} else {
			__raw_writel(mdr, sdramc_base + AT91_DDRSDRC_MDR);
			__raw_writel(lpr, sdramc_base + AT91_DDRSDRC_LPR);
			if (sdramc_base1) {
				__raw_writel(mdr, sdramc_base1 + AT91_DDRSDRC_MDR);
				__raw_writel(lpr, sdramc_base1 + AT91_DDRSDRC_LPR);
			}
		}
		break;
	case AT91_MEMCTRL_SDRAMC:
		if (is_active) {
			lpr = __raw_readl(sdramc_base + AT91_SDRAMC_LPR);

			__raw_writel((lpr & ~AT91_SDRAMC_LPCB) | AT91_SDRAMC_LPCB_SELF_REFRESH, sdramc_base + AT91_SDRAMC_LPR);
		} else {
			__raw_writel(lpr, sdramc_base + AT91_SDRAMC_LPR);
		}
		break;
	}
}

static void at91_pm_suspend_in_sram(void __iomem *pmc, void __iomem *ramc0,
				    void __iomem *ramc1, int memctrl)
{
	int memtype, pm_mode;

	memtype = memctrl & AT91_PM_MEMTYPE_MASK;
	pm_mode = (memctrl >> AT91_PM_MODE_OFFSET) & AT91_PM_MODE_MASK;

	dsb();

	at91_sramc_self_refresh(1, memtype, ramc0, ramc1);

#if defined(CONFIG_CPU_V7)
	dsb();
	wfi();
#else
	asm volatile ( "mcr	p15, 0, %0, c7, c0, 4" \
		      : : "r" (0) : "memory");
#endif

	at91_sramc_self_refresh(0, memtype, ramc0, ramc1);
}
EXPORT_SYMBOL(at91_pm_suspend_in_sram);

static void *sram_alloc(unsigned long size)
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

	sram_pool = gen_pool_get(&pdev->dev, NULL);
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

static void sram_free(void)
{
	gen_pool_free(sram_pool, sram_base, sram_size);
}

int __init test_init(void)
{
	init_module_at("atmel_pm.ko", sram_alloc);

	return 0;
}

void __exit test_exit(void)
{
	sram_free();
}

module_init(test_init);
module_exit(test_exit);

MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Your description");
MODULE_LICENSE("GPL");
