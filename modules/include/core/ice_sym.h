#ifndef ICE_SYM_H
#define ICE_SYM_H

#include <linux/types.h>

extern unsigned long g_swapper_pg_dir;

int  ice_sym_init(void);
void ice_sym_exit(void);

#endif
