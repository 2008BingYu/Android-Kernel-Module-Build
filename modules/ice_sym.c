#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/mm.h>
#include <linux/dcache.h>
#include <linux/path.h>

#include "ice_sym.h"

void (*p_flush_tlb_mm)(struct mm_struct *mm) = NULL;
pte_t *(*p_pte_offset_map)(pmd_t *pmd, unsigned long address) = NULL;
void (*p_pte_unmap)(pte_t *pte) = NULL;
char *(*p_d_path)(const struct path *path, char *buf, int buflen) = NULL;

static unsigned long (*g_kallsyms_lookup_name)(const char *name) = NULL;

static int ice_resolve_symbols(void)
{
    struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };
    int ret;

    ret = register_kprobe(&kp);
    if (ret < 0) {
        pr_err("[IceDriver] kprobe register failed: %d\n", ret);
        return ret;
    }
    g_kallsyms_lookup_name = (void *)kp.addr;
    unregister_kprobe(&kp);

    if (!g_kallsyms_lookup_name) {
        pr_err("[IceDriver] kallsyms_lookup_name not found\n");
        return -1;
    }

    p_flush_tlb_mm   = (void *)g_kallsyms_lookup_name("flush_tlb_mm");
    p_pte_offset_map = (void *)g_kallsyms_lookup_name("pte_offset_map");
    p_pte_unmap      = (void *)g_kallsyms_lookup_name("pte_unmap");
    p_d_path         = (void *)g_kallsyms_lookup_name("d_path");

    pr_info("[IceDriver] sym: flush_tlb_mm=%px pte_offset_map=%px pte_unmap=%px d_path=%px\n",
            p_flush_tlb_mm, p_pte_offset_map, p_pte_unmap, p_d_path);

    if (!p_pte_offset_map)
        pr_err("[IceDriver] pte_offset_map not found, mem rw will fail\n");

    return 0;
}

int ice_sym_init(void)
{
    return ice_resolve_symbols();
}

void ice_sym_exit(void)
{
}