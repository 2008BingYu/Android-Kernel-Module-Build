/*
 * ice_sym.c — 无符号依赖的内核页表根解析
 *
 * 真机实测：register_kprobe 偷 kallsyms_lookup_name 会在 insmod 时
 * 引发全机瞬间硬挂（stop_machine 文本 patch 路径，无任何日志落盘）。
 *
 * 因此不再使用 kprobe / 任何未导出符号：
 *   - 直接读 TTBR1_EL1 拿内核页表根的物理地址（表基址在 RAM 中，走 linear map 可得 VA）
 *   - 不受 KASLR 影响（TTBR1 存的是物理地址）
 *   - 统一解码同时兼容 48 位 PA 与 52 位 PA（LPA）两种布局：
 *       48 位 PA：TTBR[47:2] = PA[47:2]（PA[5:2] 恒为 0，表基址对齐）
 *       52 位 PA：TTBR[5:2] = PA[51:48]，TTBR[47:6] = PA[47:6]
 *     统一公式在 48 位 PA 下 TTBR[5:2]=0，自然退化成立。
 */

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
