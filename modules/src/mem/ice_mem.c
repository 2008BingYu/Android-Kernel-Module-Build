#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/pgtable.h>
#include <linux/vmalloc.h>
#include <asm/pgtable.h>
#include <asm/pgtable-prot.h>
#include <asm/memory.h>
#include <asm/barrier.h>

#include "ice_compat.h"
#include "ice_mem.h"
#include "ice_ioctl.h"
#include "ice_sym.h"

static void *trampoline_base = NULL;
static pte_t *trampoline_pte = NULL;
static pte_t trampoline_orig_pte;

static pid_t  s_last_pid = 0;
static struct mm_struct *s_last_mm = NULL;
static unsigned long s_last_vpage_base = 0;
static unsigned long s_last_ppage_base = 0;

int ice_mem_init(void)
{
    unsigned long vaddr;
    pte_t *ptep;
    unsigned int level;

    if (in_atomic()) {
        pr_err("[IceDriver] atomic context, cannot vmalloc\n");
        return -EPERM;
    }

    trampoline_base = vmalloc(PAGE_SIZE);
    if (!trampoline_base) {
        pr_err("[IceDriver] vmalloc failed\n");
        return -ENOMEM;
    }

    memset(trampoline_base, 0xAA, PAGE_SIZE);

    vaddr = (unsigned long)trampoline_base;

    if (!p_lookup_address) {
        pr_err("[IceDriver] lookup_address not available\n");
        vfree(trampoline_base);
        trampoline_base = NULL;
        return -EFAULT;
    }

    ptep = p_lookup_address(vaddr, &level);
    if (!ptep) {
        pr_err("[IceDriver] lookup_address failed\n");
        vfree(trampoline_base);
        trampoline_base = NULL;
        return -EFAULT;
    }

    if (level != 3) {
        pr_err("[IceDriver] trampoline is huge page, level=%u, unsupported\n", level);
        vfree(trampoline_base);
        trampoline_base = NULL;
        return -EFAULT;
    }

    trampoline_pte = ptep;
    trampoline_orig_pte = *ptep;

    pr_info("[IceDriver] trampoline: va=%px pte=%px level=%u\n",
            trampoline_base, trampoline_pte, level);
    return 0;
}

void ice_mem_exit(void)
{
    if (trampoline_base) {
        if (trampoline_pte) {
            set_pte(trampoline_pte, trampoline_orig_pte);
            if (p_flush_tlb_kernel_range) {
                p_flush_tlb_kernel_range((unsigned long)trampoline_base,
                                         (unsigned long)trampoline_base + PAGE_SIZE);
            }
        }
        vfree(trampoline_base);
        trampoline_base = NULL;
        trampoline_pte = NULL;
    }

    if (s_last_mm) {
        mmput(s_last_mm);
        s_last_mm = NULL;
    }
    s_last_pid = 0;
    s_last_vpage_base = 0;
    s_last_ppage_base = 0;
}

static int ice_walk_translate_va_to_pa(struct mm_struct *mm, uint64_t vaddr,
                                       phys_addr_t *paddr)
{
    pgd_t *pgd;
    p4d_t *p4d;
    pud_t *pud;
    pmd_t *pmd;
    pte_t *ptep;
    pte_t pte;
    unsigned long pfn;
    uint64_t page_mask;

    if (!mm || !paddr)
        return -EINVAL;

    pgd = pgd_offset(mm, vaddr);
    if (pgd_none(*pgd) || pgd_bad(*pgd))
        return -EFAULT;

    p4d = p4d_offset(pgd, vaddr);
    if (p4d_none(*p4d) || p4d_bad(*p4d))
        return -EFAULT;

    pud = pud_offset(p4d, vaddr);
    if (pud_none(*pud))
        return -EFAULT;

    if (pud_leaf(*pud)) {
        pfn = pud_pfn(*pud);
        page_mask = PUD_MASK;
        goto done;
    }

    if (pud_bad(*pud))
        return -EFAULT;

    pmd = pmd_offset(pud, vaddr);
    if (pmd_none(*pmd))
        return -EFAULT;

    if (pmd_leaf(*pmd)) {
        pfn = pmd_pfn(*pmd);
        page_mask = PMD_MASK;
        goto done;
    }

    if (pmd_bad(*pmd))
        return -EFAULT;

    ptep = pte_offset_kernel(pmd, vaddr);
    if (!ptep)
        return -EFAULT;

    pte = __pte(READ_ONCE(pte_val(*ptep)));
    if (!pte_present(pte))
        return -EFAULT;

    pfn = pte_pfn(pte);
    page_mask = PAGE_MASK;

done:
    if (!pfn_valid(pfn))
        return -EFAULT;

    *paddr = ((phys_addr_t)pfn << PAGE_SHIFT) + (vaddr & ~page_mask);
    return 0;
}

static int ice_trampoline_map(phys_addr_t paddr, void **mapped)
{
    unsigned long pfn;
    unsigned long offset;
    pte_t new_pte;
    unsigned long pfn_shift;

    if (!trampoline_pte || !trampoline_base)
        return -EINVAL;

    pfn = __phys_to_pfn(paddr);
    if (!pfn_valid(pfn))
        return -EFAULT;

    offset = paddr & ~PAGE_MASK;

#if defined(CONFIG_ARM64_PA_BITS_52) && CONFIG_ARM64_PA_BITS_52
    pfn_shift = 12;
#else
    pfn_shift = PAGE_SHIFT;
#endif

    new_pte = __pte((pte_val(trampoline_orig_pte) & ~PAGE_MASK)
                    | ((unsigned long)pfn << pfn_shift));

    set_pte(trampoline_pte, new_pte);

    if (p_flush_tlb_kernel_range) {
        p_flush_tlb_kernel_range((unsigned long)trampoline_base,
                                 (unsigned long)trampoline_base + PAGE_SIZE);
    }

    *mapped = (uint8_t *)trampoline_base + offset;
    return 0;
}

static int ice_rw_mem_force(struct mm_struct *mm, pid_t pid,
                            unsigned long addr, void *buffer,
                            size_t size, bool write)
{
    uint64_t current_vaddr;
    size_t bytes_processed;
    size_t bytes_done;
    int status;

    if (!buffer || size == 0)
        return -EINVAL;

    current_vaddr = untagged_addr(addr);
    bytes_processed = 0;
    bytes_done = 0;
    status = 0;

    while (bytes_processed < size) {
        size_t bytes_this_page = PAGE_SIZE - (current_vaddr & (PAGE_SIZE - 1));
        unsigned long va_base;
        unsigned long page_offset;
        phys_addr_t paddr;
        void *mapped;
        int hit_cache = 0;

        if (bytes_this_page > size - bytes_processed)
            bytes_this_page = size - bytes_processed;

        va_base = current_vaddr & PAGE_MASK;
        page_offset = current_vaddr & ~PAGE_MASK;

        if (s_last_mm == mm && s_last_vpage_base == va_base && s_last_pid == pid) {
            mapped = (uint8_t *)trampoline_base + page_offset;
            hit_cache = 1;
            goto do_io;
        }

        status = ice_walk_translate_va_to_pa(mm, current_vaddr, &paddr);
        if (status != 0)
            goto next;

        status = ice_trampoline_map(paddr, &mapped);
        if (status != 0)
            goto next;

        s_last_pid = pid;
        s_last_vpage_base = va_base;
        s_last_ppage_base = paddr & PAGE_MASK;

        if (s_last_mm != mm) {
            if (s_last_mm)
                mmput(s_last_mm);
            s_last_mm = mm;
            mmget(s_last_mm);
        }

do_io:
        if (write)
            memcpy(mapped, (uint8_t *)buffer + bytes_processed, bytes_this_page);
        else
            memcpy((uint8_t *)buffer + bytes_processed, mapped, bytes_this_page);

        bytes_done += bytes_this_page;

next:
        bytes_processed += bytes_this_page;
        current_vaddr += bytes_this_page;
    }

    return (bytes_done == 0) ? status : (int)bytes_done;
}

static struct mm_struct *ice_get_mm_by_pid(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct *mm;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)
        get_task_struct(task);
    rcu_read_unlock();

    if (!task)
        return NULL;

    mm = get_task_mm(task);
    put_task_struct(task);
    return mm;
}

int ice_read_mem(COPY_MEMORY *cm)
{
    struct mm_struct *mm;
    int ret;

    if (!cm || cm->size == 0)
        return -EINVAL;

    mm = ice_get_mm_by_pid(cm->pid);
    if (!mm)
        return -ESRCH;

    ret = ice_rw_mem_force(mm, cm->pid, cm->addr, cm->buffer, cm->size, false);
    mmput(mm);
    return ret;
}

int ice_write_mem(COPY_MEMORY *cm)
{
    struct mm_struct *mm;
    int ret;

    if (!cm || cm->size == 0)
        return -EINVAL;

    mm = ice_get_mm_by_pid(cm->pid);
    if (!mm)
        return -ESRCH;

    ret = ice_rw_mem_force(mm, cm->pid, cm->addr, cm->buffer, cm->size, true);
    mmput(mm);
    return ret;
}