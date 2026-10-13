#ifndef ICE_SYM_H
#define ICE_SYM_H

#include <linux/types.h>

/* 内核页表根（swapper_pg_dir 虚拟地址），由 ice_sym_init 从 TTBR1_EL1 推导 */
extern unsigned long g_swapper_pg_dir;

int  ice_sym_init(void);
void ice_sym_exit(void);

#endif
