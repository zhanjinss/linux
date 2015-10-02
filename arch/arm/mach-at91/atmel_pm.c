#include <linux/io.h>
#include <linux/module.h>
#include <linux/clk/at91_pmc.h>
#include <mach/at91_ramc.h>
#include "pm.h"

#define	SRAMC_SELF_FRESH_ACTIVE		0x01
#define	SRAMC_SELF_FRESH_EXIT		0x00

void __aeabi_unwind_cpp_pr0(void)
{
};

void __aeabi_unwind_cpp_pr1(void)
{
};

void __aeabi_unwind_cpp_pr2(void)
{
};

static void plopfunc(void __iomem *saddr)
{
	int i;
	char str[] = "\r\nplop\r\n\r\n";

	for (i = 0; i < sizeof(str); i++) {
		while (!(__raw_readl(saddr + 0x14) & (1 << 1)))
		{
		}
		__raw_writel(str[i], saddr + 0x1c);
	}
}
EXPORT_SYMBOL(plopfunc);

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
			__raw_writel(1, sdramc_base + AT91RM9200_SDRAMC_SRR);
		break;

	case AT91_MEMCTRL_DDRSDR:
		if (is_active) {
			mdr = __raw_readl(sdramc_base + AT91_DDRSDRC_MDR);
			lpr = __raw_readl(sdramc_base + AT91_DDRSDRC_LPR);

			if ((mdr & AT91_DDRSDRC_MD) == AT91_DDRSDRC_MD_LOW_POWER_DDR)
				__raw_writel((mdr & ~AT91_DDRSDRC_MD) | AT91_DDRSDRC_MD_DDR2, sdramc_base + AT91_DDRSDRC_MDR);
			__raw_writel((lpr & ~AT91_DDRSDRC_LPCB) | AT91_DDRSDRC_LPCB_SELF_REFRESH, sdramc_base + AT91_DDRSDRC_LPR);

#if 0
			mdr1 = __raw_readl(sdramc_base1 + AT91_DDRSDRC_MDR);
			lpr1 = __raw_readl(sdramc_base1 + AT91_DDRSDRC_LPR);
			if ((mdr1 & AT91_DDRSDRC_MD) == AT91_DDRSDRC_MD_LOW_POWER_DDR)
				__raw_writel((mdr1 & ~AT91_DDRSDRC_MD) | AT91_DDRSDRC_MD_DDR2, sdramc_base1 + AT91_DDRSDRC_MDR);
			__raw_writel((lpr1 & ~AT91_DDRSDRC_LPCB) | AT91_DDRSDRC_LPCB_SELF_REFRESH, sdramc_base1 + AT91_DDRSDRC_LPR);
#endif
		} else {
			__raw_writel(mdr, sdramc_base + AT91_DDRSDRC_MDR);
			__raw_writel(lpr, sdramc_base + AT91_DDRSDRC_LPR);
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
EXPORT_SYMBOL(at91_sramc_self_refresh);

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

MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Your description");
MODULE_LICENSE("GPL");
