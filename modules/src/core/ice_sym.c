#include <linux/module.h>
#include <linux/kernel.h>
#include <asm/memory.h>

#include "ice_sym.h"

unsigned long g_swapper_pg_dir = 0;

int ice_sym_init(void)
{
    unsigned long ttbr;
    unsigned long pgd_pa;

    asm volatile("mrs %0, ttbr1_el1" : "=r"(ttbr));

    pgd_pa = (ttbr & GENMASK_ULL(47, 6)) |
             ((ttbr & GENMASK_ULL(5, 2)) << 46);

    g_swapper_pg_dir = (unsigned long)phys_to_virt(pgd_pa);

    pr_info("[IceDriver] sym-free: ttbr1=0x%lx swapper_pg_dir=0x%lx\n",
            ttbr, g_swapper_pg_dir);

    return 0;
}

void ice_sym_exit(void)
{
}
