#ifndef ICE_SYM_H
#define ICE_SYM_H

#include <linux/types.h>
#include <linux/mm.h>
#include <linux/dcache.h>
#include <linux/path.h>

extern void (*p_flush_tlb_mm)(struct mm_struct *mm);
extern pte_t *(*p_pte_offset_map)(pmd_t *pmd, unsigned long address);
extern void (*p_pte_unmap)(pte_t *pte);
extern char *(*p_d_path)(const struct path *path, char *buf, int buflen);

int  ice_sym_init(void);
void ice_sym_exit(void);

#endif