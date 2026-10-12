#ifndef ICE_COMPAT_H
#define ICE_COMPAT_H

#include <linux/version.h>
#include <linux/mm.h>
#include <linux/device.h>
#include <linux/highmem.h>
#include <asm/pgtable-hwdef.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    #define ICE_CLASS_CREATE(name)   class_create(name)
#else
    #define ICE_CLASS_CREATE(name)   class_create(THIS_MODULE, name)
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
    #define ICE_KMAP(page)           kmap_local_page(page)
    #define ICE_KUNMAP(addr)         kunmap_local(addr)
#else
    #define ICE_KMAP(page)           kmap(page)
    #define ICE_KUNMAP(addr)         kunmap(addr)
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
    #define ICE_FOR_EACH_VMA(vma, mm) \
        { \
            VMA_ITERATOR(vmi, mm, 0); \
            for_each_vma(vmi, vma) {
    #define ICE_FOR_EACH_VMA_END() \
            } \
        }
#else
    #define ICE_FOR_EACH_VMA(vma, mm) \
        for (vma = mm->mmap; vma; vma = vma->vm_next) {
    #define ICE_FOR_EACH_VMA_END() \
        }
#endif

#endif