#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/genalloc.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/slab.h>
#include <linux/moduleloader.h>

#ifndef ARCH_SHF_SMALL
#define ARCH_SHF_SMALL 0
#endif

static struct gen_pool *sram_pool;
static unsigned long sram_base;
void __iomem *addr;

/* Update size with this section: return offset. */
static long get_offset(unsigned long *size, Elf_Shdr *sechdr)
{
	long ret;

	ret = ALIGN(*size, sechdr->sh_addralign ? : 1);
	*size = ret + sechdr->sh_size;
	return ret;
}

//static void (*plopfunc_ptr)(void __iomem *addr);

/* XXX: From module.c */
struct load_info {
	Elf_Ehdr *hdr;
	unsigned long len;
	Elf_Shdr *sechdrs;
	char *secstrings, *strtab;
	unsigned long symoffs, stroffs;
	struct _ddebug *debug;
	unsigned int num_debug;
	bool sig_ok;
	struct {
		unsigned int sym, str, mod, vers, info, pcpu;
	} index;
};


struct pie {
	const struct firmware *fw;
};

struct pie* pie_load(const char *pie_name)
{
	struct pie* pie;
	Elf_Ehdr *hdr;
	int ret;

	pie = kzalloc(sizeof(struct pie), GFP_KERNEL);
	if (!pie)
		return ERR_PTR(-ENOMEM);

	ret = request_firmware(&pie->fw, pie_name, NULL);
	if (ret < 0) {
		goto free;
	}

	hdr = (Elf_Ehdr *)pie->fw->data;
	/* From elf_header_check() */
	if (pie->fw->size < sizeof(*(hdr))) {
		ret = -ENOEXEC;
		goto free;
	}

	if (memcmp(hdr->e_ident, ELFMAG, SELFMAG) != 0
	    || hdr->e_type != ET_REL
	    || !elf_check_arch(hdr)
	    || hdr->e_shentsize != sizeof(Elf_Shdr)) {
		ret = -ENOEXEC;
		goto free;
	}

	if (hdr->e_shoff >= pie->fw->size
	    || (hdr->e_shnum * sizeof(Elf_Shdr) >
		pie->fw->size - hdr->e_shoff)) {
		ret = -ENOEXEC;
		goto free;
	}

	return pie;

free:
	kfree(pie);
	return ERR_PTR(ret);
}

/* Lay out the SHF_ALLOC sections in a way not dissimilar to how ld
   might -- code, read-only data, read-write data, small data.	Tally
   sizes, and place the offsets into sh_entsize fields: high bit means it
   belongs in init. */
static void layout_sections(struct module *mod, struct load_info *info)
{
	static unsigned long const masks[][2] = {
		/* NOTE: all executable code must be the first section
		 * in this array; otherwise modify the text_size
		 * finder in the two loops below */
		{SHF_EXECINSTR | SHF_ALLOC, ARCH_SHF_SMALL},
		{SHF_ALLOC, SHF_WRITE | ARCH_SHF_SMALL},
		{SHF_WRITE | SHF_ALLOC, ARCH_SHF_SMALL},
		{ARCH_SHF_SMALL | SHF_ALLOC, 0}
	};
	unsigned int m, i;

	for (i = 0; i < info->hdr->e_shnum; i++)
		info->sechdrs[i].sh_entsize = ~0UL;

	for (m = 0; m < ARRAY_SIZE(masks); ++m) {
		for (i = 0; i < info->hdr->e_shnum; ++i) {
			Elf_Shdr *s = &info->sechdrs[i];

			if ((s->sh_flags & masks[m][0]) != masks[m][0]
			    || (s->sh_flags & masks[m][1])
			    || s->sh_entsize != ~0UL)
				continue;
			s->sh_entsize =
				get_offset((unsigned long *)&mod->core_size, s);
		}

		if (m == 0)
			mod->core_text_size = mod->core_size;
	}
}

/* Change all symbols so that st_value encodes the pointer directly. */
static int simplify_symbols(struct module *mod, const struct load_info *info)
{
	Elf_Shdr *symsec = &info->sechdrs[info->index.sym];
	Elf_Sym *sym = (void *)symsec->sh_addr;
	unsigned long secbase;
	unsigned int i;
	int ret = 0;

	for (i = 1; i < symsec->sh_size / sizeof(Elf_Sym); i++) {
		const char *name = info->strtab + sym[i].st_name;

		switch (sym[i].st_shndx) {
		case SHN_COMMON:
			/* Ignore common symbols */
			if (!strncmp(name, "__gnu_lto", 9))
				break;

			/* We compiled with -fno-common.  These are not
			   supposed to happen.  */
			pr_debug("Common symbol: %s\n", name);
			pr_warn("%s: please compile with -fno-common\n",
			       mod->name);
			ret = -ENOEXEC;
			break;

		case SHN_ABS:
			/* Don't need to do anything */
			pr_debug("Absolute symbol: 0x%08lx\n",
			       (long)sym[i].st_value);
			break;

		case SHN_UNDEF:
			pr_warn("%s: Unknown symbol %s)\n",
				mod->name, name);
			break;

		default:
			secbase = info->sechdrs[sym[i].st_shndx].sh_addr;
			sym[i].st_value += secbase;
			break;
		}
	}

	return ret;
}

static void dump_elfsymbols(Elf_Shdr *sechdrs, unsigned int symindex,
			    const char *strtab, struct module *mod)
{
	Elf_Sym *sym = (void *)sechdrs[symindex].sh_addr;
	unsigned int i, n = sechdrs[symindex].sh_size / sizeof(Elf_Sym);

	if (sechdrs[symindex].sh_type != SHT_SYMTAB)
		return;

	for (i = 1; i < n; i++) {
		pr_dbg("%d %s at 0x%x\n", i, strtab + sym[i].st_name,
			 sym[i].st_value);
	}
}

int pie_relocate(struct pie *pie, void __iomem *dst)
{
	struct module mod;
	struct load_info inf = {};
	struct load_info *info = &inf;
	int i, err;

	memset(&mod, 0, sizeof(struct module));
	strcpy(mod.name, "PIE plop");

	info->hdr = (Elf_Ehdr *)pie->fw->data;
	info->len = pie->fw->size;
	info->sechdrs = (void *)info->hdr + info->hdr->e_shoff;
	info->secstrings = (void *)info->hdr
		+ info->sechdrs[info->hdr->e_shstrndx].sh_offset;

	for (i = 1; i < info->hdr->e_shnum; i++) {
		Elf_Shdr *shdr = &info->sechdrs[i];
		if (shdr->sh_type != SHT_NOBITS
		    && info->len < shdr->sh_offset + shdr->sh_size) {
			pr_err("Module len %lu truncated\n", info->len);
			return -ENOEXEC;
		}

		/* Mark all sections sh_addr with their address in the
		   temporary image. */
		shdr->sh_addr = (size_t)info->hdr + shdr->sh_offset;

		pr_dbg(" section sh_name %s sh_addr 0x%x\n",
			 info->secstrings + info->sechdrs[i].sh_name,
			 info->sechdrs[i].sh_addr);

		/* Internal symbols and strings. */
		if (info->sechdrs[i].sh_type == SHT_SYMTAB) {
			info->index.sym = i;
			info->index.str = info->sechdrs[i].sh_link;
			info->strtab = (char *)info->hdr
				+ info->sechdrs[info->index.str].sh_offset;
			//break;
		}
	}

	for (i = 1; i < info->hdr->e_shnum; i++)
		dump_elfsymbols(info->sechdrs, i, info->strtab, &mod);

	layout_sections(&mod, info);

	for (i = 0; i < info->hdr->e_shnum; i++) {
		void *dest;

		if (!(info->sechdrs[i].sh_flags & SHF_ALLOC))
			continue;

		dest = dst + info->sechdrs[i].sh_entsize;

		if (info->sechdrs[i].sh_type != SHT_NOBITS)
			memcpy(dest, (void *)info->sechdrs[i].sh_addr,
			       info->sechdrs[i].sh_size);
		/* Update sh_addr to point to copy in image. */
		info->sechdrs[i].sh_addr = (unsigned long)dest;

		pr_dbg(" section sh_name %s sh_addr 0x%x\n",
			 info->secstrings + info->sechdrs[i].sh_name,
			 info->sechdrs[i].sh_addr);
	}

	/* Fix up syms, so that st_value is a pointer to location. */
	err = simplify_symbols(&mod, info);
	if (err < 0)
		return err;

	/* Now do relocations. */
	for (i = 1; i < info->hdr->e_shnum; i++) {
		unsigned int infosec = info->sechdrs[i].sh_info;

		/* Not a valid relocation section? */
		if (infosec >= info->hdr->e_shnum)
			continue;

		/* Don't bother with non-allocated sections */
		if (!(info->sechdrs[infosec].sh_flags & SHF_ALLOC))
			continue;

		if (info->sechdrs[i].sh_type == SHT_REL)
			err = apply_relocate(info->sechdrs, info->strtab,
					     info->index.sym, i, &mod);
		else if (info->sechdrs[i].sh_type == SHT_RELA)
			err = apply_relocate_add(info->sechdrs, info->strtab,
						 info->index.sym, i, &mod);
		if (err < 0)
			return err;
	}

	pr_dbg("%s +%d %s\n", __FILE__, __LINE__, __func__);
	for (i = 1; i < info->hdr->e_shnum; i++)
		dump_elfsymbols(info->sechdrs, i, info->strtab, &mod);

	return 0;
}

int pie_get_sym(struct pie *pie, char *symname)
{
	Elf_Sym *sym = (void *)pie->info->sechdrs[index.sym].sh_addr;
	unsigned int i, n = pie->info->sechdrs[index.sym].sh_size / sizeof(Elf_Sym);

	if (pie->info->sechdrs[index.sym].sh_type != SHT_SYMTAB)
		return -EINVAL;

	for (i = 1; i < n; i++) {
		if (!strcmp(symname, pie->info->strtab + sym[i].st_name)) {
			pr_err("found %s at %p\n", pie->info->strtab + sym[i].st_name, sym[i].st_value)
			break;
		}
	}

	return 0;
}

int __init test_init(void)
{
	phys_addr_t sram_pbase;
	struct device_node *node;
	struct platform_device *pdev = NULL;
	void __iomem *sram_final;
	struct pie *pie;
	int ret;

	pie = pie_load("atmel_pm.pie");
	if (IS_ERR_OR_NULL(pie))
		return PTR_ERR(pie);

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

	sram_base = gen_pool_alloc(sram_pool, pie->fw->size);
	if (!sram_base) {
		pr_warn("%s: unable to alloc sram!\n", __func__);
		return -EINVAL;
	}

	sram_pbase = gen_pool_virt_to_phys(sram_pool, sram_base);
	sram_final = __arm_ioremap_exec(sram_pbase,
					pie->fw->size, false);
	if (!sram_final) {
		pr_warn("SRAM: Could not remap\n");
		return -1;
	}

	pr_err("%s +%d %s: %p %p\n", __FILE__, __LINE__, __func__, pie->fw->data, sram_final);

	ret = pie_relocate(pie, sram_final);
	if (ret)
		return ret;

	release_firmware(pie->fw);

	pie_get_sym(pie, "plopfunc");

	addr = ioremap(0xffffee00, SZ_512);

	return -ENODEV;
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

