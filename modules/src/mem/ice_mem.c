#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/percpu.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>
#include <linux/preempt.h>
#include <linux/err.h>
#include <asm/pgtable.h>
#include <asm/pgtable-prot.h>
#include <asm/memory.h>
#include <asm/barrier.h>
#include <asm/tlbflush.h>

#include "ice_compat.h"
#include "ice_ioctl.h"
#include "ice_mem.h"
#include "ice_sym.h"

#define ICE_MAX_RW_SIZE (1024 * 1024)


#define ICE_ASM_EXTABLE(from, to)                             \
    "   .pushsection    __ex_table, \"a\"\n"                  \
    "   .align          3\n"                                  \
    "   .long           (" #from " - .), (" #to " - .)\n"     \
    "   .popsection\n"

static inline int ice_copy_nofault(void *dst, const void *src, size_t size)
{
    unsigned long err = 0;

    asm volatile(
    "1: ldrb   w3, [%[src]], #1\n"
    "2: strb   w3, [%[dst]], #1\n"
    "   subs   %[size], %[size], #1\n"
    "   b.ne   1b\n"
    "   b      3f\n"
    "4: mov    %[err], #1\n"
    "3:\n"
    ICE_ASM_EXTABLE(1b, 4b)
    ICE_ASM_EXTABLE(2b, 4b)
    : [dst] "+r"(dst), [src] "+r"(src), [size] "+r"(size), [err] "+r"(err)
    : : "w3", "memory");

    return err ? -EFAULT : 0;
}


struct ice_pte_slot {
    void        *base_address;
    pte_t       *pte_address;
    struct page *placeholder_page;
};

static DEFINE_PER_CPU(struct ice_pte_slot, ice_pte_slots);

static pte_t *ice_get_kernel_pte(uint64_t vaddr)
{
    pgd_t *pgd;
    p4d_t *p4d;
    pud_t *pud;
    pmd_t *pmd;

    if (!g_swapper_pg_dir)
        return NULL;

    pgd = (pgd_t *)g_swapper_pg_dir + pgd_index(vaddr);
    if (pgd_none(*pgd) || pgd_bad(*pgd))
        return NULL;

    p4d = p4d_offset(pgd, vaddr);
    if (p4d_none(*p4d) || p4d_bad(*p4d))
        return NULL;

    pud = pud_offset(p4d, vaddr);
    if (pud_bad(*pud))
        return NULL;

    pmd = pmd_offset(pud, vaddr);
    if (pmd_bad(*pmd))
        return NULL;

    return pte_offset_kernel(pmd, vaddr);
}

static void ice_free_pte_slots(void)
{
    int cpu;

    for_each_possible_cpu(cpu) {
        struct ice_pte_slot *s = per_cpu_ptr(&ice_pte_slots, cpu);
        if (s->base_address)     vunmap(s->base_address);
        if (s->placeholder_page) __free_page(s->placeholder_page);
        memset(s, 0, sizeof(*s));
    }
}

static int ice_alloc_pte_slots(void)
{
    int cpu;

    /* STAGE0: 禁用 PTE 槽位分配（bisect 用），恢复时删掉下面两行 */
    pr_info("[IceDriver] STAGE0: pte slots skipped\n");
    return 0;

    if (in_atomic())
        return -EPERM;

    for_each_possible_cpu(cpu) {
        struct ice_pte_slot *s = per_cpu_ptr(&ice_pte_slots, cpu);
        uint64_t vaddr;
        unsigned int level;

        s->placeholder_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
        if (!s->placeholder_page) {
            ice_free_pte_slots();
            return -ENOMEM;
        }

        vaddr = (uint64_t)vmap(&s->placeholder_page, 1, VM_MAP, PAGE_KERNEL_RO);
        if (!vaddr) {
            ice_free_pte_slots();
            return -ENOMEM;
        }

        s->pte_address = ice_get_kernel_pte(vaddr);
        if (!s->pte_address && p_lookup_address) {
            level = 0;
            s->pte_address = p_lookup_address(vaddr, &level);
            if (s->pte_address && level != 3)
                s->pte_address = NULL;
        }
        if (!s->pte_address) {
            pr_err("[IceDriver] slot pte failed, cpu=%d\n", cpu);
            ice_free_pte_slots();
            return -EFAULT;
        }

        s->base_address = (void *)vaddr;
    }
    return 0;
}

static inline void ice_flush_tlb_addr_all_asid_current_cpu(uint64_t addr)
{
    uint64_t tlbi_addr = __TLBI_VADDR(addr, 0);

    asm volatile("dsb nshst\n\t"
                 "tlbi vaale1, %[tlbi_addr]\n\t"
                 "dsb nsh\n\t"
                 "isb\n\t"
                 : : [tlbi_addr] "r"(tlbi_addr) : "memory");
}

static const uint64_t ICE_PTE_FLAGS = PTE_TYPE_PAGE | PTE_VALID | PTE_AF |
                                      PTE_SHARED | PTE_PXN | PTE_UXN |
                                      PTE_ATTRINDX(MT_NORMAL_NC);

static inline void *ice_pte_map_page(phys_addr_t paddr, size_t size)
{
    struct ice_pte_slot *s = this_cpu_ptr(&ice_pte_slots);
    uint64_t pfn = __phys_to_pfn(paddr);

    if (!size)
        return ERR_PTR(-EINVAL);
    if (!s->base_address || !s->pte_address)
        return ERR_PTR(-ENODEV);
    if (!pfn_valid(pfn))
        return ERR_PTR(-EFAULT);
    if (((paddr & ~PAGE_MASK) + size) > PAGE_SIZE)
        return ERR_PTR(-EINVAL);

    set_pte(s->pte_address, pfn_pte(pfn, __pgprot(ICE_PTE_FLAGS)));
    ice_flush_tlb_addr_all_asid_current_cpu((uint64_t)s->base_address);

    return (uint8_t *)s->base_address + (paddr & ~PAGE_MASK);
}

static inline int ice_pte_read_physical(phys_addr_t paddr, void *buffer, size_t size)
{
    void *mapped;
    int status = 0;

    preempt_disable();
    mapped = ice_pte_map_page(paddr, size);
    if (IS_ERR(mapped)) status = PTR_ERR(mapped);
    else                status = ice_copy_nofault(buffer, mapped, size);
    preempt_enable();

    return status;
}

static inline int ice_pte_write_physical(phys_addr_t paddr, const void *buffer, size_t size)
{
    void *mapped;
    int status = 0;

    preempt_disable();
    mapped = ice_pte_map_page(paddr, size);
    if (IS_ERR(mapped)) status = PTR_ERR(mapped);
    else                status = ice_copy_nofault(mapped, buffer, size);
    preempt_enable();

    return status;
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

static inline int ice_linear_read_physical(phys_addr_t paddr, void *buffer, size_t size)
{
    void *kernel_vaddr = phys_to_virt(paddr);

    if (!size || !buffer)
        return -EINVAL;
    if (size > PAGE_SIZE - (paddr & ~PAGE_MASK))
        return -EINVAL;
    if (!__is_lm_address((unsigned long)kernel_vaddr))
        return -EFAULT;

    return ice_copy_nofault(buffer, kernel_vaddr, size);
}

static inline int ice_linear_write_physical(phys_addr_t paddr, const void *buffer, size_t size)
{
    void *kernel_vaddr = phys_to_virt(paddr);

    if (!buffer || !size)
        return -EINVAL;
    if ((paddr & ~PAGE_MASK) + size > PAGE_SIZE)
        return -EINVAL;
    if (!__is_lm_address((unsigned long)kernel_vaddr))
        return -EFAULT;

    return ice_copy_nofault(kernel_vaddr, buffer, size);
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

static int ice_rw_mem_force(struct mm_struct *mm, unsigned long addr,
                            void *buffer, size_t size, bool write)
{
    uint64_t current_vaddr = untagged_addr(addr);
    size_t bytes_processed = 0;
    size_t bytes_done = 0;
    int status = 0;

    if (!buffer || size == 0)
        return -EINVAL;

    while (bytes_processed < size) {
        size_t bytes_this_page = PAGE_SIZE - (current_vaddr & (PAGE_SIZE - 1));
        phys_addr_t paddr;

        if (bytes_this_page > size - bytes_processed)
            bytes_this_page = size - bytes_processed;

        status = ice_walk_translate_va_to_pa(mm, current_vaddr, &paddr);

        if (status == 0) {
            if (!write)
                status = ice_linear_read_physical(paddr,
                            (uint8_t *)buffer + bytes_processed, bytes_this_page);
            else
                status = ice_linear_write_physical(paddr,
                            (const uint8_t *)buffer + bytes_processed, bytes_this_page);

            if (status != 0) {
                if (!write)
                    status = ice_pte_read_physical(paddr,
                                (uint8_t *)buffer + bytes_processed, bytes_this_page);
                else
                    status = ice_pte_write_physical(paddr,
                                (const uint8_t *)buffer + bytes_processed, bytes_this_page);
            }
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
    void *kbuf;
    int ret;

    if (!cm || cm->size == 0)
        return -EINVAL;
    if (cm->size > ICE_MAX_RW_SIZE)
        return -EINVAL;

    kbuf = kvmalloc(cm->size, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;

    mm = ice_get_mm_by_pid(cm->pid);
    if (!mm) {
        kvfree(kbuf);
        return -ESRCH;
    }

    ret = ice_rw_mem_force(mm, cm->addr, kbuf, cm->size, false);
    mmput(mm);

    if (ret > 0 &&
        copy_to_user((void __user *)cm->buffer, kbuf, ret))
        ret = -EFAULT;

    kvfree(kbuf);
    return ret;
}

int ice_write_mem(COPY_MEMORY *cm)
{
    struct mm_struct *mm;
    void *kbuf;
    int ret;

    if (!cm || cm->size == 0)
        return -EINVAL;
    if (cm->size > ICE_MAX_RW_SIZE)
        return -EINVAL;

    kbuf = kvmalloc(cm->size, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;

    if (copy_from_user(kbuf, (void __user *)cm->buffer, cm->size)) {
        kvfree(kbuf);
        return -EFAULT;
    }

    mm = ice_get_mm_by_pid(cm->pid);
    if (!mm) {
        kvfree(kbuf);
        return -ESRCH;
    }

    ret = ice_rw_mem_force(mm, cm->addr, kbuf, cm->size, true);
    mmput(mm);

    kvfree(kbuf);
    return ret;
}

int ice_mem_init(void)
{
    int ret;

    ret = ice_alloc_pte_slots();
    if (ret) {
        pr_err("[IceDriver] pte slot alloc failed: %d\n", ret);
        return ret;
    }

    pr_info("[IceDriver] memory core ready (lsdriver scheme)\n");
    return 0;
}

void ice_mem_exit(void)
{
    ice_free_pte_slots();
}