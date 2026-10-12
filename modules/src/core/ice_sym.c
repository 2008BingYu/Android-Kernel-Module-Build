#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/mm.h>
#include <linux/dcache.h>
#include <linux/path.h>

#include "ice_sym.h"

pte_t *(*p_lookup_address)(unsigned long address, unsigned int *level) = NULL;
void (*p_flush_tlb_kernel_range)(unsigned long start, unsigned long end) = NULL;
char *(*p_d_path)(const struct path *path, char *buf, int buflen) = NULL;
unsigned long g_swapper_pg_dir = 0;

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

    p_lookup_address         = (void *)g_kallsyms_lookup_name("lookup_address");
    p_flush_tlb_kernel_range = (void *)g_kallsyms_lookup_name("flush_tlb_kernel_range");
    p_d_path                 = (void *)g_kallsyms_lookup_name("d_path");
    g_swapper_pg_dir         = g_kallsyms_lookup_name("swapper_pg_dir");

    pr_info("[IceDriver] sym: lookup_address=%px flush_tlb_kernel_range=%px "
            "d_path=%px swapper_pg_dir=%px\n",
            p_lookup_address, p_flush_tlb_kernel_range, p_d_path,
            (void *)g_swapper_pg_dir);

    if (!g_swapper_pg_dir)
        pr_err("[IceDriver] swapper_pg_dir not found, "
               "pte slot will fallback to lookup_address\n");

    return 0;
}

int ice_sym_init(void)
{
    return ice_resolve_symbols();
}

void ice_sym_exit(void)
{
}