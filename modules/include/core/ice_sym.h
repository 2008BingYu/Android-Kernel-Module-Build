#ifndef ICE_SYM_H
#define ICE_SYM_H

#include <linux/types.h>
#include <linux/mm.h>
#include <linux/dcache.h>
#include <linux/path.h>

extern pte_t *(*p_lookup_address)(unsigned long address, unsigned int *level);
extern void (*p_flush_tlb_kernel_range)(unsigned long start, unsigned long end);
extern char *(*p_d_path)(const struct path *path, char *buf, int buflen);

int  ice_sym_init(void);
void ice_sym_exit(void);

#endif