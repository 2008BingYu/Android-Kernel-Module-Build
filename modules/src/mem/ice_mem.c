#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/pgtable.h>
#include <asm/pgtable-hwdef.h>

#include "ice_compat.h"
#include "ice_mem.h"
#include "ice_ioctl.h"
#include "ice_sym.h"

static struct task_struct *ice_find_task(pid_t pid)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)
        get_task_struct(task);
    rcu_read_unlock();

    return task;
}

static pte_t *ice_walk_pte(struct mm_struct *mm, unsigned long addr)
{
    pgd_t *pgd;
    p4d_t *p4d;
    pud_t *pud;
    pmd_t *pmd;
    pte_t *pte;

    pgd = pgd_offset(mm, addr);
    if (pgd_none(*pgd) || pgd_bad(*pgd)) return NULL;

    p4d = p4d_offset(pgd, addr);
    if (p4d_none(*p4d) || p4d_bad(*p4d)) return NULL;

    pud = pud_offset(p4d, addr);
    if (pud_none(*pud) || pud_bad(*pud)) return NULL;

    pmd = pmd_offset(pud, addr);
    if (pmd_none(*pmd)) return NULL;

    if (pmd_trans_huge(*pmd) || pmd_devmap(*pmd))
        return NULL;

    if (!p_pte_offset_map)
        return NULL;

    pte = p_pte_offset_map(pmd, addr);
    if (!pte || !pte_present(*pte)) {
        if (pte && p_pte_unmap)
            p_pte_unmap(pte);
        return NULL;
    }

    return pte;
}

static int ice_rw_mem_force(struct task_struct *task, unsigned long addr,
                            void *buffer, size_t size, bool write)
{
    struct mm_struct *mm;
    size_t done;
    int ret;
    unsigned long cur_addr;
    size_t chunk;
    pte_t *pte;
    pte_t old_pte;
    pte_t new_pte;
    bool need_restore;
    struct page *page;
    void *kaddr;
    void *target;

    mm = get_task_mm(task);
    if (!mm)
        return -EINVAL;

    done = 0;
    ret = 0;

    mmap_read_lock(mm);

    while (done < size) {
        cur_addr = addr + done;
        chunk = PAGE_SIZE - (cur_addr & (PAGE_SIZE - 1));
        if (chunk > size - done)
            chunk = size - done;

        pte = ice_walk_pte(mm, cur_addr);
        if (!pte) {
            ret = -EFAULT;
            break;
        }

        old_pte = *pte;
        need_restore = false;

        if (write && !pte_write(*pte)) {
            new_pte = __pte(pte_val(*pte) | PTE_WRITE | PTE_DIRTY);
            set_pte_at(mm, cur_addr, pte, new_pte);
            if (p_flush_tlb_mm)
                p_flush_tlb_mm(mm);
            need_restore = true;
        }

        page = pte_page(*pte);
        kaddr = ICE_KMAP(page);
        target = (void *)((unsigned long)kaddr + (cur_addr & (PAGE_SIZE - 1)));

        if (write)
            memcpy(target, (char *)buffer + done, chunk);
        else
            memcpy((char *)buffer + done, target, chunk);

        ICE_KUNMAP(kaddr);

        if (need_restore) {
            set_pte_at(mm, cur_addr, pte, old_pte);
            if (p_flush_tlb_mm)
                p_flush_tlb_mm(mm);
        }

        if (p_pte_unmap)
            p_pte_unmap(pte);

        done += chunk;
    }

    mmap_read_unlock(mm);
    mmput(mm);
    return ret;
}

int ice_read_mem(COPY_MEMORY *cm)
{
    struct task_struct *task;
    int ret;

    if (!cm || cm->size == 0)
        return -EINVAL;

    task = ice_find_task(cm->pid);
    if (!task)
        return -ESRCH;

    ret = ice_rw_mem_force(task, cm->addr, cm->buffer, cm->size, false);
    put_task_struct(task);
    return ret;
}

int ice_write_mem(COPY_MEMORY *cm)
{
    struct task_struct *task;
    int ret;

    if (!cm || cm->size == 0)
        return -EINVAL;

    task = ice_find_task(cm->pid);
    if (!task)
        return -ESRCH;

    ret = ice_rw_mem_force(task, cm->addr, cm->buffer, cm->size, true);
    put_task_struct(task);
    return ret;
}