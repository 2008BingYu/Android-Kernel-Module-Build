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
#include <asm/pgtable.h>
#include <asm/pgtable-prot.h>
#include <asm/memory.h>

#include "ice_compat.h"
#include "ice_mem.h"
#include "ice_ioctl.h"

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

    if (pud_leaf(*pud)) {
        pfn = pud_pfn(*pud);
        page_mask = PUD_MASK;
        goto done;
    }

    if (pud_none(*pud) || pud_bad(*pud))
        return -EFAULT;

    pmd = pmd_offset(pud, vaddr);

    if (pmd_leaf(*pmd)) {
        pfn = pmd_pfn(*pmd);
        page_mask = PMD_MASK;
        goto done;
    }

    if (pmd_none(*pmd) || pmd_bad(*pmd))
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

static int ice_linear_read_physical(phys_addr_t paddr, void *buffer, size_t size)
{
    void *kernel_vaddr = phys_to_virt(paddr);

    if (!size || !buffer)
        return -EINVAL;

    if (size > PAGE_SIZE - (paddr & ~PAGE_MASK))
        return -EINVAL;

    if (!__is_lm_address(kernel_vaddr))
        return -EFAULT;

    memcpy(buffer, kernel_vaddr, size);
    return 0;
}

static int ice_linear_write_physical(phys_addr_t paddr, const void *buffer, size_t size)
{
    void *kernel_vaddr = phys_to_virt(paddr);

    if (!buffer || !size)
        return -EINVAL;

    if ((paddr & ~PAGE_MASK) + size > PAGE_SIZE)
        return -EINVAL;

    if (!__is_lm_address(kernel_vaddr))
        return -EFAULT;

    memcpy(kernel_vaddr, buffer, size);
    return 0;
}

static int ice_rw_mem_force(struct mm_struct *mm, unsigned long addr,
                            void *buffer, size_t size, bool write)
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
        phys_addr_t paddr;

        if (bytes_this_page > size - bytes_processed)
            bytes_this_page = size - bytes_processed;

        status = ice_walk_translate_va_to_pa(mm, current_vaddr, &paddr);

        if (status == 0) {
            if (write)
                status = ice_linear_write_physical(paddr,
                                                   (uint8_t *)buffer + bytes_processed,
                                                   bytes_this_page);
            else
                status = ice_linear_read_physical(paddr,
                                                  (uint8_t *)buffer + bytes_processed,
                                                  bytes_this_page);
        }

        if (status == 0) {
            bytes_done += bytes_this_page;
        } else if (!write && size > 8) {
            memset((uint8_t *)buffer + bytes_processed, 0, bytes_this_page);
        }

        bytes_processed += bytes_this_page;
        current_vaddr += bytes_this_page;
    }

    return (bytes_done == 0) ? status : (int)bytes_done;
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

    ret = ice_rw_mem_force(mm, cm->addr, cm->buffer, cm->size, false);
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

    ret = ice_rw_mem_force(mm, cm->addr, cm->buffer, cm->size, true);
    mmput(mm);
    return ret;
}