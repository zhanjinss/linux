#include <linux/io.h>
#include <linux/module.h>

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

MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Your description");
MODULE_LICENSE("GPL");

